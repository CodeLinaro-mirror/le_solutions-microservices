# CompoundFilter

Combine multiple filters using `and` or `or`.

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Type of operation: &#x60;and&#x60; or &#x60;or&#x60;. | 
**filters** | [**List[CompoundFilterFiltersInner]**](CompoundFilterFiltersInner.md) | Array of filters to combine. Items can be &#x60;ComparisonFilter&#x60; or &#x60;CompoundFilter&#x60;. | 

## Example

```python
from openapi_client.models.compound_filter import CompoundFilter

# TODO update the JSON string below
json = "{}"
# create an instance of CompoundFilter from a JSON string
compound_filter_instance = CompoundFilter.from_json(json)
# print the JSON string representation of the object
print(CompoundFilter.to_json())

# convert the object into a dict
compound_filter_dict = compound_filter_instance.to_dict()
# create an instance of CompoundFilter from a dict
compound_filter_from_dict = CompoundFilter.from_dict(compound_filter_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


