# Click

A click action. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | Specifies the event type. For a click action, this property is  always set to &#x60;click&#x60;.  | [default to 'click']
**button** | **str** | Indicates which mouse button was pressed during the click. One of &#x60;left&#x60;, &#x60;right&#x60;, &#x60;wheel&#x60;, &#x60;back&#x60;, or &#x60;forward&#x60;.  | 
**x** | **int** | The x-coordinate where the click occurred.  | 
**y** | **int** | The y-coordinate where the click occurred.  | 

## Example

```python
from openapi_client.models.click import Click

# TODO update the JSON string below
json = "{}"
# create an instance of Click from a JSON string
click_instance = Click.from_json(json)
# print the JSON string representation of the object
print(Click.to_json())

# convert the object into a dict
click_dict = click_instance.to_dict()
# create an instance of Click from a dict
click_from_dict = Click.from_dict(click_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


