# ResponseReasoningSummaryPartDoneEventPart

The completed summary part. 

## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The type of the summary part. Always &#x60;summary_text&#x60;. | 
**text** | **str** | The text of the summary part. | 

## Example

```python
from openapi_client.models.response_reasoning_summary_part_done_event_part import ResponseReasoningSummaryPartDoneEventPart

# TODO update the JSON string below
json = "{}"
# create an instance of ResponseReasoningSummaryPartDoneEventPart from a JSON string
response_reasoning_summary_part_done_event_part_instance = ResponseReasoningSummaryPartDoneEventPart.from_json(json)
# print the JSON string representation of the object
print(ResponseReasoningSummaryPartDoneEventPart.to_json())

# convert the object into a dict
response_reasoning_summary_part_done_event_part_dict = response_reasoning_summary_part_done_event_part_instance.to_dict()
# create an instance of ResponseReasoningSummaryPartDoneEventPart from a dict
response_reasoning_summary_part_done_event_part_from_dict = ResponseReasoningSummaryPartDoneEventPart.from_dict(response_reasoning_summary_part_done_event_part_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


